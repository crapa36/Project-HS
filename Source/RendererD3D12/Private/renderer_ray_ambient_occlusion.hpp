#pragma once
#include "renderer_ray_geometry.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_nrd.hpp"

namespace hs
{
// Established AO guide/reprojection ABI plus world camera origin and ray-cone
// controls + native hit-distance normalization. 416B, rounded to512 for CBV.
// radius/strength are gtao_parameters.xy.
struct alignas(16) RayAmbientOcclusionConstants
{
    AmbientOcclusionConstants ao;
    DirectX::XMFLOAT4 world_origin{};
    DirectX::XMFLOAT4 ray_parameters{0.002f,0.0f,0.0f,0.0f}; // normal bias m, cone spread radians
    DirectX::XMFLOAT4 nrd_parameters{3.0f,0.1f,20.0f,0.0f};
};
static_assert(sizeof(RayAmbientOcclusionConstants)==416);
static_assert(offsetof(RayAmbientOcclusionConstants,world_origin)==368);
struct RayAmbientOcclusionInputs
{
    RaySceneOutputs scene;
    RayGeometryOutputs geometry;
    std::span<const RayShaderRead> reads; // every canonical page/index/skin/material/FrameCB used at hits
    AmbientOcclusionGuide depth,normal,metadata,motion,surface_history;
    RayFrameTableBuilder make_bindings;
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{};
    CompletionSet completed{};
    bool reset_history{};
    SdkCamera camera;
    SdkFrameIdentity identity;
};
struct RayAmbientOcclusionOutputs
{
    ResourceVersion visibility;
    ID3D12Resource *visibility_resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE visibility_srv{},descriptor_suffix{};
    std::uint32_t descriptor_count{kRayDescriptorCount};
};
struct RayAmbientOcclusionStorage;
class RayAmbientOcclusionCandidate
{
    std::shared_ptr<RayAmbientOcclusionStorage> storage_;
    friend class RayAmbientOcclusionSystem;
};
class RayAmbientOcclusionSystem
{
  public:
    RayAmbientOcclusionSystem();
    ~RayAmbientOcclusionSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        ViewExtent capacity,GraphicsQuality quality,const render_data::QualityProfile &,bool enabled,std::uint32_t frame_slots,
        const RayShaderReader &,RayAmbientOcclusionCandidate &,RayDenoiserRequest denoiser={}) const;
    [[nodiscard]] Result Commit(RayAmbientOcclusionCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const RayAmbientOcclusionInputs &,
        RayAmbientOcclusionConstants,RayAmbientOcclusionOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] RayDenoiserStatus GetDenoiserStatus() const;
    // Called only after all old submissions have reliably completed.
    void SwapState(RayAmbientOcclusionSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
