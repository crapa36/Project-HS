#pragma once
#include "renderer_ambient_occlusion.hpp"
#include "renderer_feature_resolve.hpp"
#include <array>
#include <span>
#include <string>

namespace hs
{
struct AllocationResource;
inline constexpr std::uint32_t kAdaptiveComputeTileSize=8;
// Only an explicitly admitted opaque material may coarsen. Unknown IDs and
// masked/thin/advanced/high-frequency domains stay full rate; flags are derived
// from actual selected material bindings/layouts, never from a fallback closure.
struct AdaptiveShadingMaterialPolicy{std::uint32_t material_id{},eligible{};};
static_assert(sizeof(AdaptiveShadingMaterialPolicy)==8);
struct AdaptiveShadingProfile
{
    bool raster_enabled{},compute_enabled{};
    ViewExtent render_capacity;
    std::uint32_t frame_slots{3},maximum_materials{256};
    std::uint64_t byte_budget{32ull*1024*1024};
};
struct alignas(16) AdaptiveShadingConstants
{
    DirectX::XMUINT4 extent{}; // Active width/height, compute tile8, actual native VRS tile size.
    DirectX::XMFLOAT4 depth_parameters{}; // Actual P33,P34,P43,P44, reversed hardware depth.
    DirectX::XMUINT4 policy{}; // Actual material count, raster enabled, compute enabled, reserved.
};
static_assert(sizeof(AdaptiveShadingConstants)==48);
struct AdaptiveShadingInputs
{
    AmbientOcclusionGuide depth,normal,base,material,metadata,motion;
    ViewExtent render_active;
    DirectX::XMFLOAT4 depth_parameters{};
    std::span<const AdaptiveShadingMaterialPolicy> material_policy;
    std::uint32_t frame_slot{};
    bool force_full_rate{}; // Camera cut/session discontinuity; no previous-history inference.
    CompletionSet completed;
    std::vector<std::shared_ptr<const void>> owners;
};
struct AdaptiveShadingOutputs
{
    AmbientOcclusionGuide density,rate;
    std::uint32_t tile_size{kAdaptiveComputeTileSize};
    bool raster_ready{},compute_ready{};
};
struct AdaptiveShadingLayout
{
    bool valid{}, active{}, raster{}, compute{};
    ViewExtent capacity{};
    std::uint32_t native_tile_size{};
    std::array<ResourceVersion, 6> guides{};
    std::array<TextureHandle, 2> textures{};
    std::array<ResourceVersion, 2> written{};
    std::array<std::uint32_t, 2> passes{};
};
struct AdaptiveShadingStatus
{
    bool raster_ready{},compute_ready{},raster_applied{},usable{true};
    std::uint32_t vrs_tier{},vrs_tile_size{};
    std::uint64_t allocated_bytes{};
    std::string reason;
};
struct AdaptiveShadingStorage;
class AdaptiveShadingCandidate
{
  public:[[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
    [[nodiscard]] AdaptiveShadingStatus Status()const{return status_;}
  private:std::shared_ptr<AdaptiveShadingStorage> storage_;AdaptiveShadingStatus status_;bool prepared_{};friend class AdaptiveShadingSystem;
};
// Bracket exactly the selected deferred pixel draw. Destruction restores1x1
// and unbinds the image even if the draw callback exits with a failure.
class AdaptiveShadingRasterScope
{
  public:AdaptiveShadingRasterScope();~AdaptiveShadingRasterScope();
    AdaptiveShadingRasterScope(const AdaptiveShadingRasterScope &)=delete;
    AdaptiveShadingRasterScope &operator=(const AdaptiveShadingRasterScope &)=delete;
  private:ID3D12GraphicsCommandList5 *commands_{};friend class AdaptiveShadingSystem;
};
class AdaptiveShadingSystem
{
  public:
    using ShaderReader=AmbientOcclusionSystem::ShaderReader;
    AdaptiveShadingSystem();~AdaptiveShadingSystem();
    [[nodiscard]] Result Prepare(ID3D12Device *,D3D12MA::Allocator *,const AdaptiveShadingProfile &,
        const ShaderReader &,AdaptiveShadingCandidate &)const;
    [[nodiscard]] Result Commit(AdaptiveShadingCandidate &&,const CompletionSet &);
    [[nodiscard]] Result Register(RenderGraphBuilder &,const AdaptiveShadingInputs &,AdaptiveShadingOutputs &,
        AdaptiveShadingLayout *layout=nullptr);
    [[nodiscard]] Result BindRetained(RenderGraphBuilder &,const AdaptiveShadingInputs &,
        const AdaptiveShadingLayout &,AdaptiveShadingOutputs &);
    [[nodiscard]] bool MatchesLayout(const AdaptiveShadingInputs &,const AdaptiveShadingLayout &)const noexcept;
    [[nodiscard]] bool MatchesShape(const AdaptiveShadingLayout &) const noexcept;
    [[nodiscard]] Result BeginRaster(RenderPassContext &,const AdaptiveShadingOutputs &,AdaptiveShadingRasterScope &);
    [[nodiscard]] Result CompleteSubmission(const RenderGraphBuilder &,const CompletionSet &,bool);
    [[nodiscard]] Result ResetAfterCompletion(const CompletionSet &);
    [[nodiscard]] std::uint64_t AllocatedBytes()const noexcept;
    [[nodiscard]] AdaptiveShadingStatus Status()const;
    [[nodiscard]] std::array<const AllocationResource *, 2> DiagnosticResources() const noexcept;
    void SwapState(AdaptiveShadingSystem &other)noexcept{impl_.swap(other.impl_);}
  private:
    [[nodiscard]] Result RegisterFrame(RenderGraphBuilder &,const AdaptiveShadingInputs &,
        AdaptiveShadingOutputs &,AdaptiveShadingLayout *,const AdaptiveShadingLayout *);
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace hs
