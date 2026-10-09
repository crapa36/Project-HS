#pragma once
#include "renderer_ray_transport.hpp"
#include "renderer_fsr.hpp"
#include "renderer_streamline.hpp"

namespace hs
{
struct AllocationResource;

inline constexpr std::uint32_t kRayReconstructionDescriptorCount=23;
// Copies the concrete ray prefix (kRayTransportSuffixCount) and space30 suffix
// into the renderer's table32 while preserving intervening service ranges.
using RayReconstructionTableBuilder=std::function<Result(D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_CPU_DESCRIPTOR_HANDLE,RayShaderBinder &)>;
enum class RayReconstructionMode : std::uint8_t { Off, FsrStandalone, DlssFused };
struct RayReconstructionProfile
{
    RayReconstructionMode mode{RayReconstructionMode::Off};
    bool diffuse{},specular{};
    ViewExtent render_capacity,output_extent;
    float diffuse_axis_ratio{}, specular_axis_ratio{};
    GraphicsQuality diffuse_quality{GraphicsQuality::Medium},specular_quality{GraphicsQuality::Medium};
    GraphicsModeId reconstruction_quality{"quality"};
    std::uint32_t frame_slots{3};
    std::uint64_t fsr_variant{};
    std::uint64_t byte_budget{};
};
struct RayReconstructionInputs
{
    RayTransportInputs transport;
    RayReconstructionTableBuilder make_bindings;
    // Actual producer backing leases for material/geometry/guide aliases.
    std::vector<std::shared_ptr<const void>> owners;
    RayTransportTexture exposure; // Actual E[current] StructuredBuffer<float> for DLSS guide conversion.
    RayTransportTexture screen_overlay_alpha; // Optional actual raster-domain reactive producer.
};
struct RayDlssGuides
{
    RayTransportTexture diffuse_albedo,specular_albedo,normal_roughness;
    RayTransportTexture diffuse_hit_distance,specular_hit_distance,depth,motion,exposure;
    ViewExtent active;
    SdkFrameIdentity identity;
    std::shared_ptr<const void> owner;
    RayTransportTexture responsive;
};
struct RayDlssCompositeInputs
{
    RayTransportTexture noisy_color,exposure,responsive;
    // These optional tags require real producers; an opaque guide is never
    // relabelled as a transparent front closure or an SSS luminosity delta.
    RayTransportTexture transparency_overlay,color_before_transparency,sss_luminance;
    std::vector<std::shared_ptr<const void>> owners;
};
struct RayDlssOutputs
{
    RayTransportTexture raw_output,pre_exposed;
    RayDlssGuides guides;
    std::shared_ptr<const void> owner;
};
struct RayReconstructionStorage;
class RayReconstructionCandidate
{
    std::shared_ptr<RayReconstructionStorage> storage_;
    friend class RayReconstructionSystem;
};
// One concrete selected ray path. Native FSR contexts are candidate-owned per
// signal. Streamline remains the renderer's single global session: Prepare
// queries its capability but never reconfigures its live viewport.
class RayReconstructionSystem
{
  public:
    RayReconstructionSystem();
    ~RayReconstructionSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        const RayReconstructionProfile &,StreamlineSession *,FsrSession *,const RayShaderReader &,
        RayReconstructionCandidate &,RendererPipelineCache *cache=nullptr) const;
    [[nodiscard]] Result Commit(RayReconstructionCandidate &&,const CompletionSet &);
    [[nodiscard]] Result ValidateNativeBudgetAfterConfiguration(RayReconstructionCandidate &,
        const std::function<Result(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *)> &submit_and_wait) const;
    [[nodiscard]] Result ValidateNativeBudgetAfterConfiguration(
        const std::function<Result(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *)> &submit_and_wait) const;
    [[nodiscard]] Result RegisterDiffuse(RenderGraphBuilder &,const RayReconstructionInputs &,
        const RayNoisySignal &,RayTransportConstants,RayDiffuseOutputs &);
    [[nodiscard]] Result RegisterSpecular(RenderGraphBuilder &,const RayReconstructionInputs &,
        const RayNoisySignal &,RayTransportConstants,RaySpecularOutputs &);
    [[nodiscard]] Result RegisterDlssGuides(RenderGraphBuilder &,const RayReconstructionInputs &,
        const RayNoisySignal &,const RayNoisySignal &,RayTransportConstants,RayDlssGuides &);
    // Root configures the adopted SL viewport after draining previous native
    // evaluation work. This registration performs exactly one RR evaluate and
    // pass51 multiplies actual exposure once; no separate SR/TAAU is registered.
    [[nodiscard]] Result RegisterDlssFused(RenderGraphBuilder &,const RayReconstructionInputs &,
        const RayDlssGuides &,const RayDlssCompositeInputs &,RayTransportConstants,RayDlssOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    // Retains the current DLSS-fused output-resolution pre-exposed HDR
    // allocation and its graph completion/state lease for capture publication.
    [[nodiscard]] AllocationResource RetainPreExposedOutput() const;
    [[nodiscard]] RayReconstructionMode Mode() const noexcept;
    [[nodiscard]] ReconstructionAvailability Availability() const noexcept;
    [[nodiscard]] std::string_view FailureReason() const noexcept;
    void SwapState(RayReconstructionSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
