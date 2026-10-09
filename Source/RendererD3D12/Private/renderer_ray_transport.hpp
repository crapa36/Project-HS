#pragma once
#include <hs/render_data/quality_profiles.hpp>
#include "renderer_ray_geometry.hpp"
#include "renderer_ambient_occlusion.hpp"
#include "renderer_nrd.hpp"
#include "renderer_ray_ser.hpp"
#include <array>

namespace hs
{
// Append space22 to table32: b0, t0..25, u0..10; then space26 t0..1/u0..4.
// The first21 descriptors
// retain the shared space20 ABI. No root parameter or root DWORD is added.
inline constexpr std::uint32_t kRayTransportDescriptorCount=45;
inline constexpr std::uint32_t kRayTransportSuffixCount=kRayDescriptorCount+kRayTransportDescriptorCount;
struct RayTransportRevisions
{
    std::uint64_t scene{},material{},light{},probe{};
    bool operator==(const RayTransportRevisions &) const = default;
};
struct alignas(16) RayTransportConstants
{
    DirectX::XMFLOAT4X4 inverse_view_projection{},world_to_view{},projection{};
    DirectX::XMFLOAT4 render_size{},gi_size{},specular_size{},previous_gi_size{},previous_specular_size{};
    DirectX::XMFLOAT4 previous_depth_parameters{},jitter_uv{},world_origin{};
    DirectX::XMFLOAT4 gi_parameters{40.0f,0.002f,0.0f,1.0f}; // distance m, bias m, cone spread, history weight
    DirectX::XMFLOAT4 specular_parameters{80.0f,0.002f,0.15f,0.8f}; // distance, bias, screen thickness, roughness cutoff
    DirectX::XMFLOAT4 filter_parameters{0.03f,0.85f,0.9f,0.08f}; // depth tolerance, normal cosine, history blend, roughness tolerance
    DirectX::XMUINT4 gi_budget{},specular_budget{},frame_info{};
    DirectX::XMUINT4 scene_material_revisions{},light_probe_revisions{},counts{};
    std::array<DirectX::XMUINT4,16> hierarchy_levels{};
    DirectX::XMFLOAT4 nrd_parameters{3.0f,0.1f,20.0f,0.0f}; // A/B/C, native radiance method
};
static_assert(sizeof(RayTransportConstants)==736);
static_assert(offsetof(RayTransportConstants,hierarchy_levels)==464);
struct RayTransportInputs
{
    RaySceneOutputs scene;
    RayGeometryOutputs geometry;
    // Actual canonical vertex/index, skin, material/texture, FrameCB, light-list,
    // probe and DFG uses. Texture aliases shared with raster use AllShaderRead.
    std::span<const RayShaderRead> reads;
    AmbientOcclusionGuide depth,normal,metadata,motion,surface_history;
    AmbientOcclusionGuide base,material,hdr,fallback_diffuse,environment_specular,subsurface_diffuse;
    std::array<AmbientOcclusionGuide,2> sidecars{};
    std::uint32_t sidecar_plane_count{};
    // Used only by fallback lighting; real GI/reflection already traces visibility.
    AmbientOcclusionGuide ambient_visibility;
    bool ambient_visibility_enabled{};
    RayFrameTableBuilder make_bindings; // Copies the concrete exported suffix; called once per stage.
    ViewExtent render_active;
    std::uint32_t frame_slot{};
    std::uint64_t frame_index{};
    CompletionSet completed{};
    RayTransportRevisions revisions;
    bool reset_history{};
    SdkCamera camera;
    SdkFrameIdentity identity;
};
struct RayTransportTexture
{
    ResourceVersion version{};
    ID3D12Resource *resource{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
};
struct RayDiffuseOutputs
{
    // Scene-linear, material-modulated replacement, delta from exact fallback,
    // and the adjusted unscattered diffuse supplied to the existing SSS service.
    RayTransportTexture diffuse,delta,adjusted_subsurface,noisy,statistics;
};
struct RaySpecularOutputs
{
    // One fully completed SSR/DXR/probe estimator; alpha is validity, not coverage.
    RayTransportTexture specular,noisy,statistics;
};
enum class RaySignalConsumer : std::uint8_t { Existing, FsrStandalone, DlssFused };
struct RayNoisySignal
{
    // Unfiltered, unexposed Cscene, material-modulated RGB; alpha is validity.
    // metric_count RG16F contains raw secondary hit distance in metres and
    // accepted primary-component path count. It never contains NRD normalization.
    RayTransportTexture color,statistics,metric_count;
    ViewExtent active;
    bool specular{};
    std::shared_ptr<const void> owner;
};
struct RayTransportOutputs { RayTransportTexture composed; };
struct RayTransportStorage;
class RendererPipelineCache;
class RayTransportCandidate
{
    std::shared_ptr<RayTransportStorage> storage_;
    friend class RayTransportSystem;
};
class RayTransportSystem
{
  public:
    RayTransportSystem();
    ~RayTransportSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,ID3D12RootSignature *,
        bool active_gi,bool active_hybrid,GraphicsQuality gi_quality,GraphicsQuality reflection_quality,
        const render_data::QualityProfiles &,
        ViewExtent capacity,std::uint32_t frame_slots,const RayShaderReader &,RayTransportCandidate &,
        RendererPipelineCache *cache=nullptr,RayDenoiserRequest denoiser={},
        RaySignalConsumer consumer=RaySignalConsumer::Existing,RayNativeTraceSelection native={},
        std::uint64_t native_byte_budget=0,std::uint64_t old_native_bytes=0) const;
    [[nodiscard]] Result Commit(RayTransportCandidate &&,const CompletionSet &);
    [[nodiscard]] Result RegisterDiffuse(RenderGraphBuilder &,const RayTransportInputs &,
        RayTransportConstants,RayDiffuseOutputs &);
    [[nodiscard]] Result RegisterSpecular(RenderGraphBuilder &,const RayTransportInputs &,
        const RayDiffuseOutputs &,RayTransportConstants,RaySpecularOutputs &);
    [[nodiscard]] Result RegisterNoisyDiffuse(RenderGraphBuilder &,const RayTransportInputs &,
        RayTransportConstants,RayNoisySignal &,RayDiffuseOutputs *unfiltered_composition=nullptr);
    [[nodiscard]] Result RegisterNoisySpecular(RenderGraphBuilder &,const RayTransportInputs &,
        const RayDiffuseOutputs &,RayTransportConstants,RayNoisySignal &);
    [[nodiscard]] Result RegisterComposite(RenderGraphBuilder &,const RayTransportInputs &,
        const RayDiffuseOutputs &,const RaySpecularOutputs &,RayTransportConstants,RayTransportOutputs &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool success);
    void ResetHistory() noexcept;
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes() const noexcept;
    [[nodiscard]] RayDenoiserStatus GetDenoiserStatus() const;
    [[nodiscard]] RayNativeTraceSelection GetNativeTraceSelection() const noexcept;
    [[nodiscard]] RaySerStatus GetSerStatus() const;
    void SwapState(RayTransportSystem &other) noexcept { impl_.swap(other.impl_); }
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hs
